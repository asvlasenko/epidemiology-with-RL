#include "approx_binomial.h"
#include "population.h"

// Calculate average infection rate
static float calc_inf_rate(const pop_t *pop, const disease_t *dis);

// Calculate fraction of critical cases that can be hospitalized
static float calc_hosp_rate(const pop_t *pop);

error_e create_pop(pop_t **out, uint64 n_people, size_t disease_duration) {
  if (out == NULL || n_people == 0 || disease_duration == 0) {
    return ERROR_INVALID_ARGS;
  }

  pop_t *pop = (pop_t *)calloc(1, sizeof(pop_t));
  if (pop == NULL) {
    return ERROR_OUT_OF_MEMORY;
  }

  pop->n_total = n_people;
  pop->n_susceptible = n_people;
  pop->max_duration = disease_duration;

  // TODO: Setting some magic numbers here for now, should really be
  // read from a file
  pop->cr_normal = 1.0f;
  pop->cr_home = 0.4f;
  pop->cr_hospital = 0.2f;
  pop->f_critical_jobs = 0.3f;

  pop->daily_production = 160.f;
  pop->prod_symp = 0.8f;
  pop->prod_dist = 0.7f;
  pop->prod_home = 0.4f;

  uint64 *ptr =
    (uint64 *)calloc(N_POP_ARRAY_FIELDS * disease_duration, sizeof(uint64));
  if (ptr == NULL) {
    return ERROR_OUT_OF_MEMORY;
  }

  pop->n_total_active = ptr;
  pop->n_asymptomatic = &ptr[disease_duration];
  pop->n_symptomatic = &ptr[2*disease_duration];
  pop->n_critical = &ptr[3*disease_duration];

  // Some paranoia to catch bugs due to incomplete changes
  // to population data type
  uint64 *ptr_last = &pop->n_critical[disease_duration-1];
  assert(ptr_last - ptr == N_POP_ARRAY_FIELDS * disease_duration - 1);

  *out = pop;
  return ERROR_SUCCESS;
}

error_e free_pop(pop_t **pop) {
  if (pop == NULL) {
    return ERROR_INVALID_ARGS;
  }

  if (*pop == NULL) {
    return ERROR_SUCCESS;
  }

  if ((*pop)->n_total_active == NULL) {
    free(*pop);
    *pop = NULL;
    return ERROR_SUCCESS;
  }

  free((*pop)->n_total_active);
  (*pop)->n_total_active = NULL;
  free(*pop);
  *pop = NULL;
  return ERROR_SUCCESS;
}

error_e infect_pop(pop_t *pop, uint64 n_cases) {
  if (pop == NULL || pop->n_total_active == NULL) {
    return ERROR_INVALID_ARGS;
  }

  if (n_cases > pop->n_susceptible) {
    n_cases = pop->n_susceptible;
  }

  pop->n_susceptible -= n_cases;
  pop->n_infected += n_cases;
  pop->n_total_active[0] += n_cases;
  pop->n_asymptomatic[0] += n_cases;
  return ERROR_SUCCESS;
}

error_e evolve_pop(pop_t *pop, const disease_t *dis) {
  if (pop == NULL || pop->n_total_active == NULL) {
    return ERROR_INVALID_ARGS;
  }

  if (dis == NULL || dis->p_transmit == NULL) {
    return ERROR_INVALID_ARGS;
  }

  // Death rate modifier based on availability of hospital beds
  float hr = calc_hosp_rate(pop);
  float hosp_death_reduction = (1.f - hr) + hr * dis->hosp_death_reduction;

  // Update conditions and advance disease stages
  // For now, assume that everyone who reaches max_duration recovers (TODO)
  pop->n_recovered += pop->n_total_active[pop->max_duration - 1];
  pop->n_infected -= pop->n_total_active[pop->max_duration - 1];

  // Advance disease time and change population states
  if (pop->n_infected > 0) {
    for (size_t i = pop->max_duration - 1; i > 0; i--) {
      uint64 n_t = pop->n_total_active[i-1];
      uint64 n_a = pop->n_asymptomatic[i-1];
      uint64 n_s = pop->n_symptomatic[i-1];
      uint64 n_c = pop->n_critical[i-1];

      // Recoveries, state transitions and deaths
      float p_r = dis->p_recovery[i-1];
      float p_s = dis->p_symptoms[i-1];
      float p_c = dis->p_critical[i-1];
      float p_d = dis->p_death[i-1] * hosp_death_reduction;

      // Number of recovered
      uint64 r_a = 0;
      uint64 r_s = 0;
      uint64 r_c = 0;

      // Number of worsened cases
      uint64 w_a = 0;   // Asymptomatic becomes symptomatic
      uint64 w_s = 0;   // Symptomatic becomes critical
      uint64 w_c = 0;   // Critical dies

      // Estimate # of transitions by drawing from a double binomial distribution
      PASS_ERROR(approx_dbin_draw(&r_a, &w_a, p_r, p_s, n_a));
      PASS_ERROR(approx_dbin_draw(&r_s, &w_s, p_r, p_c, n_s));
      PASS_ERROR(approx_dbin_draw(&r_c, &w_c, p_r, p_d, n_c));

      // Update number of people in different categories, for this infection day
      pop->n_total_active[i] = n_t - r_a - r_s - r_c - w_c;
      pop->n_asymptomatic[i] = n_a - r_a - w_a;
      pop->n_symptomatic[i] = n_s + w_a - r_s - w_s;
      pop->n_critical[i] = n_c + w_s - r_c - w_c;

      pop->n_dead += w_c;
      pop->n_recovered += r_a + r_s + r_c;
      pop->n_infected -= r_a + r_s + r_c + w_c;

      // DEBUG: check conservation by time bin
      assert(pop->n_total_active[i] ==
        pop->n_asymptomatic[i] + pop->n_symptomatic[i] + pop->n_critical[i]);
    }
  } // If pop(n_infected > 0)

  // Day 0 bin: calculate number of newly infected
  // TODO: impact of control measures
  pop->n_total_active[0] = 0;
  pop->n_asymptomatic[0] = 0;
  pop->n_symptomatic[0] = 0;
  pop->n_critical[0] = 0;

  float infection_rate = calc_inf_rate(pop, dis);
  uint64 n_infected;
  PASS_ERROR(approx_bin_draw(&n_infected,
                        infection_rate / (float)pop->n_susceptible,
                        pop->n_susceptible));
  PASS_ERROR(infect_pop(pop, n_infected));

  // DEBUG: Check conservation by entire population
  assert(pop->n_total == pop->n_susceptible + pop->n_infected
    + pop->n_recovered + pop->n_dead + pop->n_vaccinated);

  return ERROR_SUCCESS;
}

error_e add_hosp_capacity(pop_t *pop, uint64 n_beds) {
  if (pop == NULL) {
    return ERROR_INVALID_ARGS;
  }
  pop->n_hospital_beds += n_beds;
  return ERROR_SUCCESS;
}

// Temporary debug function, print to command line for testing
error_e print_pop_info(size_t t, const pop_t *pop) {
  if (pop == NULL) {
    return ERROR_INVALID_ARGS;
  }

  printf("%I64u %I64u %I64u %I64u %I64u %I64u %f %f\n",
    t,
    pop->n_total,
    pop->n_susceptible,
    pop->n_infected,
    pop->n_recovered,
    pop->n_dead,
    hospital_load(pop),
    productivity_loss(pop)/1e9
  );

  return ERROR_SUCCESS;
}

static float calc_inf_rate(const pop_t *pop, const disease_t *dis) {

  // Weights for how often asymptomatic, symptomatic and critical people come
  // into contact with each other.  These are affected by the population's
  // social distancing policy.

  // 1 is the baseline rate, corresponding to transmission rates as given
  // in the disease data file.

  // TODO: once testing and quarantine is implemented, add weights for those
  // who are known positive and / or quarantined.

  float wa = pop->cr_normal;
  float ws = 0.5 * (pop->cr_normal + pop->cr_home);
  float wc = pop->cr_home;
  float wh = pop->cr_hospital;

  float fcj = pop->f_critical_jobs;

  if (pop->policy.dist_home_all) {
    // People without critical jobs stay at home, and even those with
    // critical jobs spend more time at home than usual
    wa = pop->cr_home * (1.f - fcj)
       + 0.5 * (pop->cr_normal + pop->cr_home) * fcj;
    ws = pop->cr_home;
  }
  else {
    if (pop->policy.dist_home_symp) {
      ws = pop->cr_home;
    }
    if (pop->policy.dist_recommend) {
      wa = 0.5 * (pop->cr_normal + pop->cr_home);
    }
  }

  // Fraction of critical patients who are hospitalized
  float hr = calc_hosp_rate(pop);

  // Modify critical transmission rate based on hospitalization fraction
  wc = hr * wh + (1.f - hr) * wc;

  // Estimated contact rate for entire population, where 1 is a baseline
  // amount of a single person's contacts per day
  float cr = wa * pop->n_susceptible + wa * pop->n_recovered;

  for (size_t i = 0; i < pop->max_duration; i++) {
    cr += wa * pop->n_asymptomatic[i] + ws * pop->n_symptomatic[i]
        + wc * pop->n_critical[i];
  }

  // Fraction of contacts that are susceptible
  float fs = wa * pop->n_susceptible / cr;

  float inf_rate = 0.f;
  assert(pop->max_duration == dis->max_duration);
  for (size_t i = 0; i < pop->max_duration; i++) {
    // Number of contacts by people on day i of disease
    float ci = wa * dis->asymp_trans_reduction * pop->n_asymptomatic[i]
      + ws * pop->n_symptomatic[i] + wc * pop->n_critical[i];

    inf_rate += dis->p_transmit[i] * ci;
  }
  inf_rate *= fs;

  return inf_rate;
}

static float calc_hosp_rate(const pop_t *pop) {
  if (!pop->n_hospital_beds) {
    return 0.f;
  }

  if (pop->n_hospital_beds >= pop->n_infected) {
    return 1.f;
  }

  float load = hospital_load(pop);
  if (load < 1.f) {
    return 1.f;
  }
  return 1.f / load;
}

float hospital_load(const pop_t *pop) {
  uint64 n_crit = 0;
  for (size_t i = 0; i < pop->max_duration; i++) {
    n_crit += pop->n_critical[i];
  }

  return (float)n_crit / (float)pop->n_hospital_beds;
}

float productivity_loss(const pop_t *pop) {
  // People who are dead or in critical condition lose all production
  uint64 n_incap = pop->n_dead;
  for (size_t i = 0; i < pop->max_duration; i++) {
    n_incap += pop->n_critical[i];
  }
  float result = (float)n_incap;

  // People who are symptomatic but not critical lose some of their production,
  // depending on policy
  uint64 n_symp = 0;
  for (size_t i = 0; i < pop->max_duration; i++) {
    n_symp += pop->n_symptomatic[i];
  }
  float ps;
  if (pop->policy.dist_home_symp) {
    ps = pop->prod_home * pop->prod_symp;
  } else {
    // TODO: 0.5 here is a magic number, consider making it a parameter
    // that can be read from a file
    ps = 0.5f * (1.f + pop->prod_home) * pop->prod_symp;
  }
  result += n_symp * (1.f - ps);

  // How productive is everyone else, depending on policy?
  uint64 n_asymp = pop->n_total - n_incap - n_symp;
  float pa = 1.f;
  if (pop->policy.dist_home_all) {
    pa = 1.f * pop->f_critical_jobs
       + pop->prod_home * (1.f - pop->f_critical_jobs);
  }
  else if (pop->policy.dist_recommend) {
    pa = 1.f * pop->f_critical_jobs
       + pop->prod_dist * (1.f - pop->f_critical_jobs);
  }
  result += n_asymp * (1.f - pa);

  return result * pop->daily_production;
}
